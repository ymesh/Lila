#include "tex/NodeDef.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "gn/Group.h"
#include "gn/Routing.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace lila::tex
{
namespace fs = std::filesystem;

namespace
{
constexpr const char* kKindNames[] = {"slang", "graph", "native", "buffer", "output", "groupInput", "groupOutput", "group", "route"};

std::optional<double> readOptNumber(const json& o, const char* key)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_number())
        return std::nullopt;
    return it->get<double>();
}

void writeOptNumber(json& o, const char* key, const std::optional<double>& v)
{
    if (v)
        o[key] = *v;
}

/** Names a body can't use for an input or param: Slang keywords and the generated function's own parameters. */
bool reservedId(std::string_view id)
{
    static const std::set<std::string, std::less<>> words = {
        "uv",       "p",        "in",      "out",      "inout",   "uniform", "const",    "static",  "sample",  "centroid",
        "struct",   "return",   "if",      "else",     "for",     "while",   "do",       "switch",  "case",    "default",
        "break",    "continue", "discard", "true",     "false",   "this",    "int",      "uint",    "float",   "half",
        "double",   "bool",     "matrix",  "vector",   "register","packoffset", "linear", "nointerpolation", "precise",
        "groupshared", "shared", "volatile", "row_major", "column_major", "typedef", "namespace", "import", "export",
        "interface", "extension", "associatedtype", "let", "var", "func", "property", "public", "private", "internal",
        "texel",    "gU",       "gPass",   "gArena",   "gOut",    "triangle", "point",   "line",    "lineadj", "triangleadj"};
    const bool prelude = id.size() > 3 && id.starts_with("tex") && id[3] >= 'A' && id[3] <= 'Z';
    return words.contains(id) || prelude || id.starts_with("S_") || id.starts_with("s_");
}

PortDecl portFromJson(const json& j, const std::string& type)
{
    PortDecl p;
    p.id = j.at("id").get<std::string>();
    if (!gn::validIdentifier(p.id))
        throw std::invalid_argument(type + ": port id '" + p.id + "' isn't an identifier");
    if (!parsePortType(j.value("type", "color"), &p.type))
        throw std::invalid_argument(type + "." + p.id + ": unknown port type " + j.value("type", ""));
    p.defaultValue = j.value("default", json());
    p.label = j.value("label", std::string());
    p.doc = j.value("doc", std::string());
    p.taps = std::max(1, j.value("taps", 1));
    p.reach = std::max(0, j.value("reach", 0));
    p.implicit = j.value("implicit", std::string());
    if (!p.implicit.empty() && p.implicit != "uv")
        throw std::invalid_argument(type + "." + p.id + ": unknown implicit " + p.implicit);
    return p;
}

json portJson(const PortDecl& p, bool input)
{
    json o = {{"id", p.id}, {"type", portTypeName(p.type)}};
    if (input && !p.defaultValue.is_null())
        o["default"] = p.defaultValue;
    if (!p.label.empty())
        o["label"] = p.label;
    if (!p.doc.empty())
        o["doc"] = p.doc;
    if (input && p.taps != 1)
        o["taps"] = p.taps;
    if (input && p.reach != 0)
        o["reach"] = p.reach;
    if (input && !p.implicit.empty())
        o["implicit"] = p.implicit;
    return o;
}

/** The value a param takes when nothing sets it. */
json typeDefault(const ParamDecl& p)
{
    switch (p.type)
    {
    case ParamType::Float: return 0.0;
    case ParamType::Int:
    case ParamType::Seed: return 0;
    case ParamType::Bool: return false;
    case ParamType::Enum: return p.options.empty() ? json("") : json(p.options[0]);
    case ParamType::Color: return json::array({0.5, 0.5, 0.5, 1.0});
    case ParamType::Gradient: return json{{"stops", json::array({json::array({0.0, json::array({0, 0, 0, 1})}), json::array({1.0, json::array({1, 1, 1, 1})})})}};
    case ParamType::Curve: return json{{"points", json::array({json::array({0.0, 0.0}), json::array({1.0, 1.0})})}};
    case ParamType::Image:
    case ParamType::Code:
    case ParamType::Node: return "";
    }
    return nullptr;
}

ParamDecl paramFromJson(const json& j, const std::string& type)
{
    ParamDecl p;
    p.id = j.at("id").get<std::string>();
    if (!gn::validIdentifier(p.id))
        throw std::invalid_argument(type + ": param id '" + p.id + "' isn't an identifier");
    if (!parseParamType(j.value("type", "float"), &p.type))
        throw std::invalid_argument(type + "." + p.id + ": unknown param type " + j.value("type", ""));
    p.options = j.value("options", std::vector<std::string>());
    if (p.type == ParamType::Enum && p.options.empty())
        throw std::invalid_argument(type + "." + p.id + ": an enum needs options");
    p.defaultValue = j.contains("default") ? j["default"] : typeDefault(p);
    if (!paramValueValid(p.type, p.defaultValue, p.options))
        throw std::invalid_argument(type + "." + p.id + ": default " + p.defaultValue.dump() + " doesn't fit a " + paramTypeName(p.type));
    p.label = j.value("label", std::string());
    p.doc = j.value("doc", std::string());
    p.panel = j.value("panel", std::string());
    p.min = readOptNumber(j, "min");
    p.max = readOptNumber(j, "max");
    p.softMin = readOptNumber(j, "softMin");
    p.softMax = readOptNumber(j, "softMax");
    p.step = readOptNumber(j, "step");
    p.hidden = j.value("hidden", false);
    return p;
}

json paramJson(const ParamDecl& p)
{
    json o = {{"id", p.id}, {"type", paramTypeName(p.type)}, {"default", p.defaultValue}};
    if (!p.label.empty())
        o["label"] = p.label;
    if (!p.doc.empty())
        o["doc"] = p.doc;
    if (!p.panel.empty())
        o["panel"] = p.panel;
    if (!p.options.empty())
        o["options"] = p.options;
    if (p.hidden)
        o["hidden"] = true;
    writeOptNumber(o, "min", p.min);
    writeOptNumber(o, "max", p.max);
    writeOptNumber(o, "softMin", p.softMin);
    writeOptNumber(o, "softMax", p.softMax);
    writeOptNumber(o, "step", p.step);
    return o;
}

/** Group Input, Group Output and Group: their sockets come from the graph they sit in or use. */
std::vector<NodeDef> coreDefs()
{
    std::vector<NodeDef> out;
    NodeDef gi;
    gi.type = "GroupInput";
    gi.label = "Group Input";
    gi.category = "Group";
    gi.doc = "The graph's port inputs, as outputs inside it.";
    gi.kind = NodeKind::GroupInput;
    gi.cost = 0;
    out.push_back(gi);
    NodeDef go;
    go.type = "GroupOutput";
    go.label = "Group Output";
    go.category = "Group";
    go.doc = "What the graph hands back to the node using it.";
    go.kind = NodeKind::GroupOutput;
    go.cost = 0;
    out.push_back(go);
    NodeDef g;
    g.type = "Group";
    g.label = "Group";
    g.category = "Group";
    g.doc = "Runs a graph from the set's subgroups, named by its group value.";
    g.kind = NodeKind::Group;
    g.cost = 0;
    out.push_back(g);
    NodeDef r;
    r.type = gn::kRerouteType;
    r.label = "Reroute";
    r.category = "Layout";
    r.doc = "A point a wire passes through, to steer it. Takes the type of whatever feeds it and can feed several inputs.";
    r.kind = NodeKind::Route;
    r.cost = 0;
    out.push_back(r);
    NodeDef send = r;
    send.type = gn::kSendType;
    send.label = "Send";
    send.doc = "Names the value wired into it, so Receive nodes anywhere in the graph can read it without a wire.";
    out.push_back(send);
    NodeDef receive = r;
    receive.type = gn::kReceiveType;
    receive.label = "Receive";
    receive.doc = "Reads the value of the Send node with its name, without a wire. Takes that value's type.";
    out.push_back(receive);
    for (NodeDef& d : out)
        d.tier = "core";
    return out;
}

struct Tier
{
    std::map<std::string, NodeDef, std::less<>> nodes;
    std::map<std::string, HelperDef, std::less<>> helpers;
};

struct LibraryState
{
    bool loaded = false;
    Tier builtin, user, workspace;
    fs::path workspaceRoot;
    std::map<std::string, NodeDef, std::less<>> core;
    std::map<std::string, HelperDef, std::less<>> extensionHelpers;
    std::vector<std::string> errors;
};

LibraryState& state()
{
    static LibraryState s;
    return s;
}

/**
 * Texture graphs plan on worker threads while the main thread opens workspaces and registers extension nodes, and either
 * may (re)scan the folders: every read and write of the library goes through this.
 */
std::recursive_mutex& libraryMutex()
{
    static std::recursive_mutex m;
    return m;
}

std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::invalid_argument("can't read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

HelperDef readHelper(const fs::path& path)
{
    HelperDef h;
    h.name = path.stem().string();
    h.code = readText(path);
    std::istringstream lines(h.code);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.starts_with("//! requires:"))
            continue;
        std::string rest = line.substr(13);
        std::string name;
        for (char c : rest + ",")
        {
            if (c == ',' || c == ' ' || c == '\t' || c == '\r')
            {
                if (!name.empty())
                    h.needs.push_back(name);
                name.clear();
            }
            else
                name += c;
        }
    }
    return h;
}

void scanDir(const fs::path& dir, const std::string& tier, Tier& into, std::vector<std::string>& errors)
{
    into = {};
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        const std::string ext = f.extension().string();
        if (ext != ".lilatexnode" && ext != ".lilatexnodes")
            continue;
        try
        {
            for (NodeDef& d : readNodeFile(f))
            {
                d.tier = tier;
                into.nodes[d.type] = std::move(d);
            }
        }
        catch (const std::exception& e)
        {
            errors.push_back(paths::toUtf8(f) + ": " + e.what());
            log::warn("texture node {}: {}", paths::toUtf8(f), e.what());
        }
    }
    fs::path helpers = dir / "helpers";
    if (fs::is_directory(helpers, ec))
        for (const auto& e : fs::directory_iterator(helpers, ec))
            if (e.path().extension() == ".slang")
            {
                try
                {
                    HelperDef h = readHelper(e.path());
                    into.helpers[h.name] = std::move(h);
                }
                catch (const std::exception& ex)
                {
                    errors.push_back(paths::toUtf8(e.path()) + ": " + ex.what());
                }
            }
}

fs::path userDir() { return PrefStore::get().userDir() / "texnodes"; }

fs::path workspaceDir()
{
    if (!Workspace::get().isOpen())
        return {};
    return Workspace::get().stateDir(false) / "texnodes";
}

LibraryState& loaded()
{
    std::scoped_lock lock(libraryMutex());
    LibraryState& s = state();
    if (s.core.empty())
        for (NodeDef& d : coreDefs())
            s.core[d.type] = std::move(d);
    if (!s.loaded)
    {
        s.errors.clear();
        scanDir(paths::resourcesDir() / "texnodes", "builtin", s.builtin, s.errors);
        // Layer compositing, mesh inputs and generators: built-in too, the node library's own types win.
        Tier generators;
        scanDir(paths::resourcesDir() / "texgenerators", "builtin", generators, s.errors);
        s.builtin.nodes.merge(generators.nodes);
        s.builtin.helpers.merge(generators.helpers);
        scanDir(userDir(), "user", s.user, s.errors);
        s.workspaceRoot = Workspace::get().isOpen() ? Workspace::get().root() : fs::path();
        if (!s.workspaceRoot.empty())
            scanDir(workspaceDir(), "workspace", s.workspace, s.errors);
        else
            s.workspace = {};
        s.loaded = true;
    }
    else
    {
        fs::path root = Workspace::get().isOpen() ? Workspace::get().root() : fs::path();
        if (root != s.workspaceRoot)
        {
            s.workspaceRoot = root;
            if (root.empty())
                s.workspace = {};
            else
                scanDir(workspaceDir(), "workspace", s.workspace, s.errors);
        }
    }
    return s;
}
} // namespace

const char* nodeKindName(NodeKind k) { return kKindNames[size_t(k)]; }

const PortDecl* NodeDef::input(std::string_view id) const
{
    for (const PortDecl& p : inputs)
        if (p.id == id)
            return &p;
    return nullptr;
}

const PortDecl* NodeDef::output(std::string_view id) const
{
    for (const PortDecl& p : outputs)
        if (p.id == id)
            return &p;
    return nullptr;
}

const ParamDecl* NodeDef::param(std::string_view id) const
{
    for (const ParamDecl& p : params)
        if (p.id == id)
            return &p;
    return nullptr;
}

json NodeDef::toJson() const
{
    json o = {{"format", "lilatexnode"}, {"version", 1}, {"type", type}, {"label", label}, {"category", category}, {"kind", nodeKindName(kind)}};
    if (!doc.empty())
        o["doc"] = doc;
    if (!keywords.empty())
        o["keywords"] = keywords;
    json ins = json::array(), outs = json::array(), prms = json::array();
    for (const PortDecl& p : inputs)
        ins.push_back(portJson(p, true));
    for (const PortDecl& p : outputs)
        outs.push_back(portJson(p, false));
    for (const ParamDecl& p : params)
        prms.push_back(paramJson(p));
    o["inputs"] = ins;
    o["outputs"] = outs;
    o["params"] = prms;
    o["cost"] = cost;
    if (!helpers.empty())
        o["helpers"] = helpers;
    if (!externals.empty())
        o["externals"] = externals;
    if (!code.empty())
        o["code"] = code;
    if (!body.empty())
        o["body"] = body;
    if (!graph.is_null())
        o["graph"] = graph;
    return o;
}

NodeDef NodeDef::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a node definition must be an object");
    NodeDef d;
    try
    {
        d.type = j.at("type").get<std::string>();
        if (!gn::validIdentifier(d.type))
            throw std::invalid_argument("type '" + d.type + "' isn't an identifier");
        d.label = j.value("label", d.type);
        d.category = j.value("category", std::string("Other"));
        d.doc = j.value("doc", std::string());
        d.keywords = j.value("keywords", std::vector<std::string>());
        std::string kind = j.value("kind", j.contains("graph") ? "graph" : "slang");
        auto k = std::find(std::begin(kKindNames), std::end(kKindNames), kind);
        if (k == std::end(kKindNames) || k - std::begin(kKindNames) >= int(NodeKind::GroupInput))
            throw std::invalid_argument(d.type + ": unknown kind " + kind);
        d.kind = NodeKind(k - std::begin(kKindNames));
        for (const json& p : j.value("inputs", json::array()))
            d.inputs.push_back(portFromJson(p, d.type));
        for (const json& p : j.value("outputs", json::array()))
            d.outputs.push_back(portFromJson(p, d.type));
        for (const json& p : j.value("params", json::array()))
            d.params.push_back(paramFromJson(p, d.type));
        d.cost = std::max(0, j.value("cost", 1));
        d.helpers = j.value("helpers", std::vector<std::string>());
        d.externals = j.value("externals", std::vector<std::string>());
        d.code = j.value("code", std::string());
        if (j.contains("body"))
        {
            if (!j["body"].is_object())
                throw std::invalid_argument(d.type + ": body must map output ids to Slang code");
            for (auto& [k2, v] : j["body"].items())
                d.body[k2] = v.get<std::string>();
        }
        d.graph = j.value("graph", json());
        std::vector<std::string> ids;
        for (const PortDecl& p : d.inputs)
            ids.push_back(p.id);
        for (const ParamDecl& p : d.params)
            ids.push_back(p.id);
        for (const std::string& e : d.externals)
        {
            if (!gn::validIdentifier(e))
                throw std::invalid_argument(d.type + ": external '" + e + "' isn't an identifier");
            ids.push_back(e);
        }
        std::vector<std::string> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            throw std::invalid_argument(d.type + ": inputs and params need distinct ids");
        for (const std::string& id : ids)
            if (reservedId(id))
                throw std::invalid_argument(d.type + ": '" + id + "' is reserved in Slang, pick another id");
        switch (d.kind)
        {
        case NodeKind::Slang:
            if (d.outputs.empty())
                throw std::invalid_argument(d.type + ": needs at least one output");
            for (const PortDecl& o : d.outputs)
                if (!d.body.contains(o.id))
                    throw std::invalid_argument(d.type + ": no body for output " + o.id);
            break;
        case NodeKind::Graph:
            if (!d.graph.is_object())
                throw std::invalid_argument(d.type + ": a graph node needs a graph object");
            break;
        case NodeKind::Buffer:
            if (d.inputs.size() != 1 || d.outputs.size() != 1)
                throw std::invalid_argument(d.type + ": a buffer node has one input and one output");
            break;
        case NodeKind::Native:
            if (d.outputs.size() != 1)
                throw std::invalid_argument(d.type + ": a native node has one output");
            break;
        default: break;
        }
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad node definition: ") + e.what());
    }
    return d;
}

std::vector<NodeDef> readNodeFile(const fs::path& path)
{
    json j;
    try
    {
        j = json::parse(readText(path));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(e.what());
    }
    std::vector<NodeDef> out;
    if (j.is_object() && j.contains("nodes"))
    {
        for (const json& n : j["nodes"])
        {
            out.push_back(NodeDef::fromJson(n));
            out.back().file = path;
        }
        return out;
    }
    out.push_back(NodeDef::fromJson(j));
    out.back().file = path;
    return out;
}

const NodeDef* findNode(std::string_view type)
{
    std::scoped_lock lock(libraryMutex());
    LibraryState& s = loaded();
    if (auto it = s.core.find(type); it != s.core.end())
        return &it->second;
    if (auto it = s.workspace.nodes.find(type); it != s.workspace.nodes.end())
        return &it->second;
    if (auto it = s.user.nodes.find(type); it != s.user.nodes.end())
        return &it->second;
    if (const NodeDef* d = extensionNodes().find(type))
        return d;
    if (auto it = s.builtin.nodes.find(type); it != s.builtin.nodes.end())
        return &it->second;
    return nullptr;
}

std::vector<const NodeDef*> allNodes()
{
    std::scoped_lock lock(libraryMutex());
    LibraryState& s = loaded();
    std::map<std::string, const NodeDef*, std::less<>> byType;
    for (auto& [k, d] : s.builtin.nodes)
        byType[k] = &d;
    for (const NodeDef* d : extensionNodes().all())
        byType[d->type] = d;
    for (auto& [k, d] : s.user.nodes)
        byType[k] = &d;
    for (auto& [k, d] : s.workspace.nodes)
        byType[k] = &d;
    for (auto& [k, d] : s.core)
        byType[k] = &d;
    std::vector<const NodeDef*> out;
    for (auto& [k, d] : byType)
        out.push_back(d);
    std::stable_sort(out.begin(), out.end(), [](const NodeDef* a, const NodeDef* b) {
        return a->category != b->category ? a->category < b->category : a->label < b->label;
    });
    return out;
}

const HelperDef* findHelper(std::string_view name)
{
    std::scoped_lock lock(libraryMutex());
    LibraryState& s = loaded();
    for (Tier* t : {&s.workspace, &s.user})
        if (auto it = t->helpers.find(name); it != t->helpers.end())
            return &it->second;
    if (auto it = s.extensionHelpers.find(name); it != s.extensionHelpers.end())
        return &it->second;
    if (auto it = s.builtin.helpers.find(name); it != s.builtin.helpers.end())
        return &it->second;
    return nullptr;
}

std::vector<fs::path> nodeDirs()
{
    std::vector<fs::path> out = {paths::resourcesDir() / "texnodes", userDir()};
    fs::path ws = workspaceDir();
    if (!ws.empty())
        out.push_back(ws);
    return out;
}

void reloadLibrary()
{
    std::scoped_lock lock(libraryMutex());
    state().loaded = false;
}

std::vector<std::string> libraryErrors()
{
    std::scoped_lock lock(libraryMutex());
    return loaded().errors;
}

Registry<NodeDef>& extensionNodes()
{
    static Registry<NodeDef> r;
    return r;
}

uint64_t registerExtensionNode(ext::Context& ctx, NodeDef def)
{
    def.tier = "extension";
    def.owner = ctx.owner();
    std::string type = def.type;
    extensionNodes().put(type, std::move(def), ctx.owner());
    std::string own = ctx.owner();
    return ctx.track("texnode:" + type, [type, own] {
        if (const std::string* o = extensionNodes().ownerOf(type); o && *o == own)
            extensionNodes().remove(type);
    });
}

size_t registerExtensionDir(ext::Context& ctx, const fs::path& dir)
{
    Tier t;
    std::vector<std::string> errors;
    scanDir(dir, "extension", t, errors);
    for (const std::string& e : errors)
        ctx.log(LogLevel::Warning, e);
    size_t n = 0;
    for (auto& [type, d] : t.nodes)
    {
        registerExtensionNode(ctx, std::move(d));
        ++n;
    }
    std::scoped_lock lock(libraryMutex());
    for (auto& [name, h] : t.helpers)
    {
        state().extensionHelpers[name] = h;
        std::string key = name;
        ctx.track("texhelper:" + key, [key] {
            std::scoped_lock unload(libraryMutex());
            state().extensionHelpers.erase(key);
        });
    }
    return n;
}
} // namespace lila::tex
