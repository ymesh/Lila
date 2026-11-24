#include "tex/Graph.h"

#include <set>
#include <stdexcept>

namespace lila::tex
{
namespace
{
void putOpt(json& o, const char* key, const std::optional<double>& v)
{
    if (v)
        o[key] = *v;
}

std::optional<double> getOpt(const json& o, const char* key)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_number())
        return std::nullopt;
    return it->get<double>();
}

json inputJson(const GraphInput& in)
{
    json o = {{"name", in.name},
              {"kind", in.param ? "param" : "port"},
              {"type", in.param ? paramTypeName(in.paramType) : portTypeName(in.port)},
              {"default", in.defaultValue}};
    for (auto [key, val] : {std::pair{"label", &in.label}, {"description", &in.description}, {"panel", &in.panel}})
        if (!val->empty())
            o[key] = *val;
    putOpt(o, "min", in.min);
    putOpt(o, "max", in.max);
    putOpt(o, "softMin", in.softMin);
    putOpt(o, "softMax", in.softMax);
    putOpt(o, "step", in.step);
    if (!in.options.empty())
        o["options"] = in.options;
    if (!in.targets.empty())
        o["targets"] = in.targets;
    return o;
}

GraphInput inputFromJson(const json& in)
{
    GraphInput gi;
    gi.name = in.at("name").get<std::string>();
    std::string type = in.value("type", "float");
    std::string kind = in.value("kind", std::string());
    PortType pt;
    ParamType prm;
    bool isPort = parsePortType(type, &pt);
    bool isParam = parseParamType(type, &prm);
    if (kind.empty())
        kind = in.contains("targets") || !isPort ? "param" : "port";
    if (kind == "param")
    {
        if (!isParam)
            throw std::invalid_argument("input " + gi.name + ": unknown parameter type " + type);
        gi.param = true;
        gi.paramType = prm;
    }
    else if (kind == "port")
    {
        if (!isPort)
            throw std::invalid_argument("input " + gi.name + ": unknown port type " + type);
        gi.port = pt;
    }
    else
        throw std::invalid_argument("input " + gi.name + ": kind must be port or param");
    gi.defaultValue = in.value("default", json());
    gi.label = in.value("label", std::string());
    gi.description = in.value("description", std::string());
    gi.panel = in.value("panel", std::string());
    gi.min = getOpt(in, "min");
    gi.max = getOpt(in, "max");
    gi.softMin = getOpt(in, "softMin");
    gi.softMax = getOpt(in, "softMax");
    gi.step = getOpt(in, "step");
    gi.options = in.value("options", std::vector<std::string>());
    gi.targets = in.value("targets", std::vector<std::string>());
    return gi;
}
} // namespace

json Graph::toJson() const
{
    json jin = json::array();
    for (const GraphInput& in : inputs)
        jin.push_back(inputJson(in));
    json jn = json::array();
    for (const GNode& n : nodes)
    {
        json o = {{"name", n.name}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
        if (!n.values.empty())
            o["values"] = n.values;
        gn::putNodeFlags(n, o);
        jn.push_back(std::move(o));
    }
    json jl = json::array();
    for (const GLink& l : links)
        jl.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
    json out = {{"name", name}, {"typeVersion", typeVersion}, {"inputs", jin}, {"nodes", jn}, {"links", jl}};
    if (!ties.empty())
        out["ties"] = gn::tiesToJson(ties);
    if (!frames.empty())
        out["frames"] = gn::framesToJson(frames);
    if (!bookmarks.empty())
        out["bookmarks"] = bookmarks;
    if (!outputs.empty())
    {
        json jo = json::array();
        for (const GraphOutput& o : outputs)
        {
            json e = {{"name", o.name}, {"type", portTypeName(o.type)}};
            if (!o.description.empty())
                e["description"] = o.description;
            jo.push_back(std::move(e));
        }
        out["outputs"] = std::move(jo);
    }
    if (!description.empty())
        out["description"] = description;
    if (!subgroups.empty())
        out["subgroups"] = subgroups;
    if (!presets.empty())
        out["presets"] = presets;
    if (seed != 0)
        out["seed"] = seed;
    return out;
}

Graph Graph::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("texture graph must be an object");
    Graph g;
    try
    {
        g.name = j.at("name").get<std::string>();
        g.typeVersion = j.value("typeVersion", 1);
        g.description = j.value("description", std::string());
        for (const json& in : j.value("inputs", json::array()))
            g.inputs.push_back(inputFromJson(in));
        for (const json& o : j.value("outputs", json::array()))
        {
            GraphOutput go;
            go.name = o.at("name").get<std::string>();
            if (!parsePortType(o.value("type", "color"), &go.type))
                throw std::invalid_argument("output " + go.name + ": unknown type " + o.value("type", ""));
            go.description = o.value("description", std::string());
            g.outputs.push_back(std::move(go));
        }
        for (const json& n : j.value("nodes", json::array()))
        {
            GNode gn;
            gn.name = n.at("name").get<std::string>();
            gn.type = n.at("type").get<std::string>();
            if (n.contains("position") && n["position"].is_array() && n["position"].size() == 2)
                gn.position = {n["position"][0].get<double>(), n["position"][1].get<double>()};
            gn.values = n.value("values", json::object());
            if (!gn.values.is_object())
                throw std::invalid_argument("node " + gn.name + ": values must be an object");
            gn::getNodeFlags(gn, n);
            g.nodes.push_back(std::move(gn));
        }
        for (const json& l : j.value("links", json::array()))
            g.links.push_back({l.at(0).get<std::string>(), l.at(1).get<std::string>(), l.at(2).get<std::string>(), l.at(3).get<std::string>()});
        g.ties = gn::tiesFromJson(j.value("ties", json::array()));
        g.frames = gn::framesFromJson(j.value("frames", json::array()));
        g.bookmarks = gn::cleanBookmarks(j.value("bookmarks", json::object()));
        if (auto it = j.find("subgroups"); it != j.end() && it->is_object())
            for (auto& [k, v] : it->items())
                g.subgroups[k] = v;
        if (auto it = j.find("presets"); it != j.end() && it->is_array())
            g.presets = *it;
        g.seed = j.value("seed", 0);
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad texture graph: ") + e.what());
    }
    return g;
}

GNode* Graph::node(std::string_view n)
{
    for (GNode& x : nodes)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GNode* Graph::node(std::string_view n) const
{
    for (const GNode& x : nodes)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GraphInput* Graph::input(std::string_view n) const
{
    for (const GraphInput& x : inputs)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GraphOutput* Graph::output(std::string_view n) const
{
    for (const GraphOutput& x : outputs)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GNode* Graph::firstOfType(std::string_view type) const
{
    for (const GNode& x : nodes)
        if (x.type == type)
            return &x;
    return nullptr;
}

const GNode* repeatOutputOf(const Graph& g, const GNode& input)
{
    if (input.type != kRepeatInputType)
        return nullptr;
    auto it = input.values.find("output");
    if (it == input.values.end() || !it->is_string())
        return nullptr;
    const GNode* out = g.node(it->get<std::string>());
    return out && out->type == kRepeatOutputType ? out : nullptr;
}

const GNode* repeatInputOf(const Graph& g, const GNode& output)
{
    for (const GNode& n : g.nodes)
        if (repeatOutputOf(g, n) == &output)
            return &n;
    return nullptr;
}

std::vector<std::string> repeatBody(const Graph& g, const std::string& input, const std::string& output)
{
    auto reach = [&](const std::string& from, bool down) {
        std::set<std::string> seen;
        std::vector<std::string> stack = {from};
        while (!stack.empty())
        {
            const std::string cur = stack.back();
            stack.pop_back();
            for (const GLink& l : g.links)
            {
                const std::string& next = down ? l.toNode : l.fromNode;
                if ((down ? l.fromNode : l.toNode) == cur && seen.insert(next).second)
                    stack.push_back(next);
            }
        }
        return seen;
    };
    const std::set<std::string> below = reach(input, true), above = reach(output, false);
    std::vector<std::string> out;
    for (const GNode& n : g.nodes)
        if (n.name != input && n.name != output && below.contains(n.name) && above.contains(n.name))
            out.push_back(n.name);
    return out;
}

std::string Graph::uniqueNodeName(std::string_view base) const
{
    std::string b(base);
    if (!node(b))
        return b;
    for (int i = 1;; ++i)
    {
        std::string c = b + std::to_string(i);
        if (!node(c))
            return c;
    }
}

std::vector<GLink> Graph::linksInto(std::string_view n, std::string_view socket) const
{
    std::vector<GLink> out;
    for (const GLink& l : links)
        if (l.toNode == n && l.toSocket == socket)
            out.push_back(l);
    return out;
}

Graph Graph::makeMaterial(std::string name)
{
    Graph g;
    g.name = std::move(name);
    g.nodes.push_back({"Output", kMaterialOutputType, {300.0, 0.0}, json::object()});
    return g;
}

Graph Graph::makeGroup(std::string name)
{
    Graph g;
    g.name = std::move(name);
    g.outputs.push_back({"out", PortType::Color, {}});
    g.nodes.push_back({"GroupInput", kGroupInputType, {-300.0, 0.0}, json::object()});
    g.nodes.push_back({"GroupOutput", kGroupOutputType, {300.0, 0.0}, json::object()});
    return g;
}
} // namespace lila::tex
